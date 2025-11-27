#include <errno.h>
#include <string.h>

#include "s3_internal.h"
#include "s3/curl_easy_factory.h"
#include "s3/alloc.h"
#include "s3/parser.h"

/*
 * Конкретная реализация backend'а на curl_easy.
 */
struct s3_http_easy_backend {
    struct s3_http_backend_impl base;
};

/* ----------------- маппинг ошибок CURL/HTTP -> s3_error ----------------- */

static s3_error_code_t
s3_http_map_curl_error(CURLcode cc)
{
    if (cc == CURLE_OK)
        return S3_E_OK;

    switch (cc) {
    case CURLE_OPERATION_TIMEDOUT:
        return S3_E_TIMEOUT;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_CONNECT:
        return S3_E_INIT;
    case CURLE_READ_ERROR:
    case CURLE_WRITE_ERROR:
        return S3_E_IO;
    default:
        return S3_E_CURL;
    }
}

static s3_error_code_t
s3_http_map_http_status(long status)
{
    if (status >= 200 && status < 300)
        return S3_E_OK;
    if (status == 404)
        return S3_E_NOT_FOUND;
    if (status == 403)
        return S3_E_ACCESS_DENIED;
    if (status == 401)
        return S3_E_AUTH;
    if (status == 408)
        return S3_E_TIMEOUT;
    return S3_E_HTTP;
}

/*
 * Общий helper: выполнить curl_easy_perform и заполнить s3_error_t.
 */
static s3_error_code_t
s3_http_easy_perform(s3_easy_handle_t *h,
                     s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    CURL *easy = h->easy;
    long http_status = 0;

    CURLcode cc = curl_easy_perform(easy);
    s3_error_code_t code = s3_http_map_curl_error(cc);

    if (cc != CURLE_OK) {
        s3_error_set(err, code, curl_easy_strerror(cc),
                     0, 0, (long)cc);
        return code;
    }

    if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &http_status) != CURLE_OK) {
        s3_error_set(err, S3_E_INTERNAL,
                     "Failed to get HTTP response code", 0, 0, 0);
        return S3_E_INTERNAL;
    }

    code = s3_http_map_http_status(http_status);
    if (code != S3_E_OK) {
        char msg[64];
        snprintf(msg, sizeof(msg), "HTTP status %ld", http_status);
        s3_error_set(err, code, msg, 0, (int)http_status, 0);
        return code;
    }

    s3_error_clear(err);
    err->code = S3_E_OK;
    err->http_status = (int)http_status;
    return S3_E_OK;
}

/* ----------------- реализация vtable: PUT / GET ----------------- */

static s3_error_code_t
s3_http_easy_put_fd(struct s3_http_backend_impl *backend,
                    const s3_put_opts_t *opts,
                    int fd, off_t offset, size_t size,
                    s3_error_t *error)
{
    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (fd < 0 || size == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd or size for PUT", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code = s3_easy_factory_new_put_fd(client, opts, fd, offset, size, &h, err);
    if (code != S3_E_OK)
        return code;

    code = s3_http_easy_perform(h, err);

    s3_easy_handle_destroy(h);

    return code;
}

static s3_error_code_t
s3_http_easy_get_fd(struct s3_http_backend_impl *backend,
                    const s3_get_opts_t *opts,
                    int fd, off_t offset, size_t max_size,
                    size_t *bytes_written,
                    s3_error_t *error)
{
    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (fd < 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd for GET", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code = s3_easy_factory_new_get_fd(client, opts, fd, offset, max_size, &h, err);
    if (code != S3_E_OK)
        return code;

    code = s3_http_easy_perform(h, err);

    if (bytes_written != NULL)
        *bytes_written = h->write_bytes_total;

    s3_easy_handle_destroy(h);
    return code;
}

static s3_error_code_t
s3_http_easy_create_bucket(struct s3_http_backend_impl *backend,
                   const s3_create_bucket_opts_t *opts,
                   s3_error_t *error)
{
    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (opts == NULL || opts->bucket == NULL || (opts->bucket)[0] == '\0') {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "bucket name is empty", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code = s3_easy_factory_new_create_bucket(client, opts, &h, err);
    if (code != S3_E_OK)
        return code;

    code = s3_http_easy_perform(h, err);

    s3_easy_handle_destroy(h);

    return code;
}

static s3_error_code_t
s3_http_easy_list_objects(struct s3_http_backend_impl *backend,
                          const s3_list_objects_opts_t *opts,
                          s3_list_objects_result_t *out,
                          s3_error_t *error)
{
    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (opts == NULL || out == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "opts or out is NULL for LIST", 0, 0, 0);
        return err->code;
    }

    memset(out, 0, sizeof(*out));

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code = s3_easy_factory_new_list_objects(client, opts, &h, err);
    if (code != S3_E_OK) {
        return code;
    }

    code = s3_http_easy_perform(h, err);

    /* После perform ответ лежит в h->owned_resp. */
    s3_mem_buf_t *resp = &h->owned_resp;
    const char *xml = resp->data ? resp->data : "";

    if (code == S3_E_OK) {
        code = s3_parse_list_response(client, xml, out, err);
    }

    s3_easy_handle_destroy(h);

    return code;
}

static s3_error_code_t
s3_http_easy_delete_objects(struct s3_http_backend_impl *backend,
                            const s3_delete_objects_opts_t *opts,
                            s3_error_t *error)
{
    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;  

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (opts == NULL || opts->objects == NULL || opts->count == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "empty delete_objects opts", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code = s3_easy_factory_new_delete_objects(client, opts, &h, err);
    if (code != S3_E_OK) {
        return code;
    }

    code = s3_http_easy_perform(h, err);

    /* Если нужен разбор ответа — смотрим в h->owned_resp. */
    if (code != S3_E_OK && h->owned_resp.data && h->owned_resp.size > 0) {
        fprintf(stderr,
                "[s3] delete_objects resp (%zu bytes):\n%.*s\n",
                h->owned_resp.size,
                (int)h->owned_resp.size,
                h->owned_resp.data);
        /* Тут можно вызвать s3_try_parse_and_log_error_xml(h->owned_resp.data, h->owned_resp.size, err); */
    }

    s3_easy_handle_destroy(h);

    return code;
}

/* ----------------- destroy + фабрика backend'а ----------------- */

static void
s3_http_easy_destroy(struct s3_http_backend_impl *backend)
{
    if (backend == NULL)
        return;

    struct s3_http_easy_backend *eb = (struct s3_http_easy_backend *)backend;
    s3_client_t *client = eb->base.client;

    if (client != NULL)
        s3_free(&client->alloc, eb);
}

/* vtable для easy backend'а */

static const struct s3_http_backend_vtbl s3_http_easy_vtbl = {
    .put_fd          = s3_http_easy_put_fd,
    .get_fd          = s3_http_easy_get_fd,
    .create_bucket   = s3_http_easy_create_bucket,
    .list_objects    = s3_http_easy_list_objects,
    .delete_objects  = s3_http_easy_delete_objects, 
    .destroy         = s3_http_easy_destroy,
};

struct s3_http_backend_impl *
s3_http_easy_backend_new(struct s3_client *client, s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (client == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client is NULL in s3_http_easy_backend_new", 0, 0, 0);
        return NULL;
    }

    struct s3_http_easy_backend *eb =
        (struct s3_http_easy_backend *)s3_alloc(&client->alloc, sizeof(*eb));
    if (eb == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_http_easy_backend", ENOMEM, 0, 0);
        return NULL;
    }

    memset(eb, 0, sizeof(*eb));
    eb->base.vtbl = &s3_http_easy_vtbl;
    eb->base.client = client;

    return &eb->base;
}
