#include <curl/curl.h>
#include <pthread.h>
#include <string.h>
#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <stdio.h>

#include "s3_internal.h"
#include "s3/curl_easy_factory.h"
#include "s3/alloc.h"

typedef struct s3_http_multi_backend s3_http_multi_backend_t;

/*
 * Запрос, который обрабатывается в multi-потоке.
 * Живёт в heap'e, coio-воркер ждёт его завершения.
 */
struct s3_multi_req {
    struct s3_multi_req *next;

    s3_easy_handle_t *easy;

    /* Результат */
    s3_error_t err;
    s3_error_code_t code;
    size_t bytes_written;
    long http_status;
    long curl_code;

    int done; /* 0/1: выставляется в multi-потоке */
};

/*
 * Backend curl_multi: отдельный поток + очередь запросов.
 */
struct s3_http_multi_backend {
    struct s3_http_backend_impl base;

    CURLM *multi;
    pthread_t thread;

    pthread_mutex_t mutex;
    pthread_cond_t cond;

    int stop; /* запрос на остановку */

    struct s3_multi_req *pending_head;
    struct s3_multi_req *pending_tail;

    int running; /* сколько easy сейчас внутри CURLM */
};

/* --------- маппинг ошибок --------- */
/* TODO: сделать общий модуль */

static s3_error_code_t
s3_http_map_curl_error_multi(CURLcode cc)
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
s3_http_map_http_status_multi(long status)
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

/* --------- вспомогательные функции --------- */

static void
s3_multi_req_init(struct s3_multi_req *req, s3_easy_handle_t *easy)
{
    memset(req, 0, sizeof(*req));
    req->easy = easy;
    s3_error_clear(&req->err);
    req->code = S3_E_OK;
}

static void
s3_multi_backend_wakeup(s3_http_multi_backend_t *mb)
{
    pthread_cond_broadcast(&mb->cond);
}

/*
 * Перенос pending-запросов в CURLM с блокировкой.
 * Здесь можно контролировать максимум inflight, если понадобится.
 */
static void
s3_multi_backend_flush_pending_locked(s3_http_multi_backend_t *mb)
{
    while (mb->pending_head != NULL) {
        struct s3_multi_req *req = mb->pending_head;
        mb->pending_head = req->next;
        if (mb->pending_head == NULL)
            mb->pending_tail = NULL;
        req->next = NULL;

        s3_easy_handle_t *eh = req->easy;
        if (eh == NULL || eh->easy == NULL) {
            s3_error_set(&req->err, S3_E_INTERNAL,
                         "Invalid easy handle in multi request",
                         0, 0, 0);
            req->code = S3_E_INTERNAL;
            req->done = 1;
            continue;
        }

        CURL *easy = eh->easy;

        /* Привязываем req к easy через PRIVATE. */
        curl_easy_setopt(easy, CURLOPT_PRIVATE, (void *)req);

        CURLMcode mc = curl_multi_add_handle(mb->multi, easy);
        if (mc != CURLM_OK) {
            s3_error_set(&req->err, S3_E_CURL,
                         curl_multi_strerror(mc),
                         0, 0, (long)mc);
            req->code = S3_E_CURL;
            req->done = 1;
            continue;
        }

        mb->running++;
    }
}

/*
 * Обработка завершившихся easy-хендлов.
 */
static void
s3_multi_backend_process_done(s3_http_multi_backend_t *mb)
{
    int msgs_in_queue = 0;
    CURLMsg *msg = NULL;

    while ((msg = curl_multi_info_read(mb->multi, &msgs_in_queue)) != NULL) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL *easy = msg->easy_handle;
        CURLcode cc = msg->data.result;

        struct s3_multi_req *req = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&req);
        if (req == NULL) {
            curl_multi_remove_handle(mb->multi, easy);
            continue;
        }

        long http_status = 0;
        s3_error_code_t code = s3_http_map_curl_error_multi(cc);
        char buf[128] = {0};

        if (cc == CURLE_OK) {
            if (curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE,
                                  &http_status) == CURLE_OK) {
                code = s3_http_map_http_status_multi(http_status);
                if (code != S3_E_OK) {
                    snprintf(buf, sizeof(buf), "HTTP status %ld", http_status);
                }
            } else {
                code = S3_E_INTERNAL;
                snprintf(buf, sizeof(buf),
                         "Failed to get HTTP response code");
            }
        } else {
            snprintf(buf, sizeof(buf), "%s", curl_easy_strerror(cc));
        }

        curl_multi_remove_handle(mb->multi, easy);

        pthread_mutex_lock(&mb->mutex);

        req->http_status = http_status;
        req->curl_code = (long)cc;

        if (code == S3_E_OK) {
            s3_error_clear(&req->err);
        } else {
            s3_error_set(&req->err, code,
                         (buf[0] ? buf : NULL),
                         0, http_status, (long)cc);
        }

        req->code = code;

        if (req->easy != NULL)
            req->bytes_written = req->easy->write_bytes_total;

        req->done = 1;

        mb->running--;

        s3_multi_backend_wakeup(mb);
        pthread_mutex_unlock(&mb->mutex);
    }
}

/* --------- поток curl_multi --------- */

static void *
s3_multi_thread_main(void *arg)
{
    s3_http_multi_backend_t *mb = (s3_http_multi_backend_t *)arg;

    for (;;) {
        pthread_mutex_lock(&mb->mutex);

        /* Ждём, пока не появятся pending или не останутся running. */
        while (!mb->stop &&
               mb->pending_head == NULL &&
               mb->running == 0) {
            pthread_cond_wait(&mb->cond, &mb->mutex);
        }

        /* Если попросили остановиться и ничего не обрабатываем — выходим. */
        if (mb->stop && mb->pending_head == NULL && mb->running == 0) {
            pthread_mutex_unlock(&mb->mutex);
            break;
        }

        /* Переносим pending в CURLM. */
        s3_multi_backend_flush_pending_locked(mb);

        pthread_mutex_unlock(&mb->mutex);

        /* Один шаг multi. */
        int still_running = 0;
        CURLMcode mc;

        do {
            mc = curl_multi_perform(mb->multi, &still_running);
        } while (mc == CURLM_CALL_MULTI_PERFORM);

        if (mc != CURLM_OK && mc != CURLM_BAD_HANDLE) {
            /* Можно залогировать, но не валимся. */
        }

        /* Обрабатываем завершённые запросы. */
        s3_multi_backend_process_done(mb);

        if (still_running > 0) {
            /* Ждём событий/таймаута, но с коротким таймаутом. */
            int numfds = 0;
            mc = curl_multi_poll(mb->multi, NULL, 0, mb->base.client->multi_idle_timeout_ms, &numfds);
            (void)mc;

            /* Ещё раз обработать завершившиеся за время poll. */
            s3_multi_backend_process_done(mb);
        }
    }

    return NULL;
}

/* --------- submit + wait для coio-воркера --------- */

static s3_error_code_t
s3_http_multi_submit_and_wait(s3_http_multi_backend_t *mb,
                              s3_easy_handle_t *easy,
                              size_t *bytes_written,
                              s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    struct s3_multi_req *req =
        (struct s3_multi_req *)malloc(sizeof(*req));
    if (req == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Out of memory in multi backend",
                     ENOMEM, 0, 0);
        return S3_E_NOMEM;
    }

    s3_multi_req_init(req, easy);

    pthread_mutex_lock(&mb->mutex);

    if (mb->stop) {
        pthread_mutex_unlock(&mb->mutex);
        free(req);
        s3_error_set(err, S3_E_INTERNAL,
                     "S3 multi backend is stopping",
                     0, 0, 0);
        return S3_E_INTERNAL;
    }

    /* Кладём в pending-очередь. */
    req->next = NULL;
    if (mb->pending_tail == NULL) {
        mb->pending_head = mb->pending_tail = req;
    } else {
        mb->pending_tail->next = req;
        mb->pending_tail = req;
    }

    s3_multi_backend_wakeup(mb);

    /* Ждём завершения. */
    while (!req->done) {
        pthread_cond_wait(&mb->cond, &mb->mutex);
    }

    pthread_mutex_unlock(&mb->mutex);

    if (error != NULL)
        *error = req->err;

    if (bytes_written != NULL)
        *bytes_written = req->bytes_written;

    s3_error_code_t rc = req->code;

    free(req);
    s3_easy_handle_destroy(easy);

    return rc;
}

/* --------- реализация vtable: PUT / GET --------- */

static s3_error_code_t
s3_http_multi_put_fd(struct s3_http_backend_impl *backend,
                     const s3_put_opts_t *opts,
                     int fd, off_t offset, size_t size,
                     s3_error_t *error)
{
    s3_http_multi_backend_t *mb = (s3_http_multi_backend_t *)backend;
    s3_client_t *client = mb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (fd < 0 || size == 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd or size for PUT", 0, 0, 0);
        return err->code;
    }

    s3_easy_io_t io;
    io.fd = fd;
    io.offset = offset;
    io.size_limit = size;

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code =
        s3_easy_factory_new_put(client, opts, &io, &h, err);
    if (code != S3_E_OK)
        return code;

    return s3_http_multi_submit_and_wait(mb, h, NULL, err);
}

static s3_error_code_t
s3_http_multi_get_fd(struct s3_http_backend_impl *backend,
                     const s3_get_opts_t *opts,
                     int fd, off_t offset, size_t max_size,
                     size_t *bytes_written,
                     s3_error_t *error)
{
    s3_http_multi_backend_t *mb = (s3_http_multi_backend_t *)backend;
    s3_client_t *client = mb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (fd < 0) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "invalid fd for GET", 0, 0, 0);
        return err->code;
    }

    s3_easy_io_t io;
    io.fd = fd;
    io.offset = offset;
    io.size_limit = max_size; /* 0 -> без ограничения */

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code =
        s3_easy_factory_new_get(client, opts, &io, &h, err);
    if (code != S3_E_OK)
        return code;

    s3_error_code_t rc =
        s3_http_multi_submit_and_wait(mb, h, bytes_written, err);

    return rc;
}

static s3_error_code_t
s3_http_multi_create_bucket(struct s3_http_backend_impl *backend,
                   const s3_create_bucket_opts_t *opts,
                   s3_error_t *error)
{
    s3_http_multi_backend_t *mb = (s3_http_multi_backend_t *)backend;
    s3_client_t *client = mb->base.client;

    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (opts->bucket == NULL || (opts->bucket)[0] == '\0') {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "bucket name is empty", 0, 0, 0);
        return err->code;
    }

    s3_easy_handle_t *h = NULL;
    s3_error_code_t code =
        s3_easy_factory_new_create_bucket(client, opts, &h, err);
    if (code != S3_E_OK)
        return code;

    s3_error_code_t rc =
        s3_http_multi_submit_and_wait(mb, h, NULL, err);

    return rc;
}

/* --------- destroy + фабрика backend'а --------- */

static void
s3_http_multi_destroy(struct s3_http_backend_impl *backend)
{
    if (backend == NULL)
        return;

    s3_http_multi_backend_t *mb = (s3_http_multi_backend_t *)backend;
    s3_client_t *client = mb->base.client;

    pthread_mutex_lock(&mb->mutex);
    mb->stop = 1;
    s3_multi_backend_wakeup(mb);
    pthread_mutex_unlock(&mb->mutex);

    pthread_join(mb->thread, NULL);

    if (mb->multi != NULL)
        curl_multi_cleanup(mb->multi);

    pthread_mutex_destroy(&mb->mutex);
    pthread_cond_destroy(&mb->cond);

    /* Теоретически pending не должно быть, но если есть — чистим. */
    struct s3_multi_req *req = mb->pending_head;
    while (req != NULL) {
        struct s3_multi_req *next = req->next;
        if (req->easy != NULL)
            s3_easy_handle_destroy(req->easy);
        free(req);
        req = next;
    }

    if (client != NULL)
        s3_free(&client->alloc, mb);
}

/* vtable для мульти-бекенда */

static const struct s3_http_backend_vtbl s3_http_multi_vtbl = {
    .put_fd        = s3_http_multi_put_fd,
    .get_fd        = s3_http_multi_get_fd,
    .create_bucket = s3_http_multi_create_bucket,
    .destroy       = s3_http_multi_destroy,
};

struct s3_http_backend_impl *
s3_http_multi_backend_new(struct s3_client *client, s3_error_t *error)
{
    s3_error_t local_err = S3_ERROR_INIT;
    s3_error_t *err = error ? error : &local_err;

    if (client == NULL) {
        s3_error_set(err, S3_E_INVALID_ARG,
                     "client is NULL in s3_http_multi_backend_new", 0, 0, 0);
        return NULL;
    }

    s3_http_multi_backend_t *mb =
        (s3_http_multi_backend_t *)s3_alloc(&client->alloc, sizeof(*mb));
    if (mb == NULL) {
        s3_error_set(err, S3_E_NOMEM,
                     "Failed to allocate s3_http_multi_backend",
                     ENOMEM, 0, 0);
        return NULL;
    }

    memset(mb, 0, sizeof(*mb));
    mb->base.vtbl = &s3_http_multi_vtbl;
    mb->base.client = client;

    mb->multi = curl_multi_init();
    if (mb->multi == NULL) {
        s3_error_set(err, S3_E_INIT,
                     "curl_multi_init failed", 0, 0, 0);
        s3_free(&client->alloc, mb);
        return NULL;
    }

    if (client->max_total_connections > 0) {
       curl_multi_setopt(mb->multi, CURLMOPT_MAX_TOTAL_CONNECTIONS,
                       (long)client->max_total_connections);
    }
    if (client->max_connections_per_host > 0) {
       curl_multi_setopt(mb->multi, CURLMOPT_MAX_HOST_CONNECTIONS,
                       (long)client->max_connections_per_host);
    }

    pthread_mutex_init(&mb->mutex, NULL);
    pthread_cond_init(&mb->cond, NULL);

    int rc = pthread_create(&mb->thread, NULL,
                            s3_multi_thread_main, mb);
    if (rc != 0) {
        s3_error_set(err, S3_E_INIT,
                     "pthread_create failed in multi backend",
                     rc, 0, 0);
        pthread_mutex_destroy(&mb->mutex);
        pthread_cond_destroy(&mb->cond);
        curl_multi_cleanup(mb->multi);
        s3_free(&client->alloc, mb);
        return NULL;
    }

    s3_error_clear(err);
    return &mb->base;
}
