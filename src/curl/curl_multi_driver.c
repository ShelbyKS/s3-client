/* src/curl/curl_multi_driver.c */

#include <s3/client.h>
#include <s3/reactor.h>

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>

#include "client_internal.h"

/* Интеграция libcurl multi API с внешним reactor'ом.
 *
 * Ответственность этого модуля:
 *
 *  - создавать/очищать CURLM (через s3_client_multi_init/cleanup)
 *  - регистрировать socket/timer callbacks в CURLM:
 *      * CURLMOPT_SOCKETFUNCTION  → s3_curl_multi_socket_cb()
 *      * CURLMOPT_TIMERFUNCTION   → s3_curl_multi_timer_cb()
 *  - через s3_reactor_t подписываться на события fd и таймеры:
 *      * io_subscribe / io_update / io_unsubscribe
 *      * timer_start / timer_cancel
 *  - по событиям reactor'а вызывать curl_multi_socket_action()
 *  - отслеживать завершённые запросы через curl_multi_info_read()
 *      * извлекать s3_request_t из CURLINFO_PRIVATE
 *      * вызывать req->done_cb(req, &err, user_data)
 *      * удалять easy из multi и освобождать ресурсы
 *
 * Текущий статус:
 *
 *  ✔ CURLM инициализируется и хранится внутри s3_client
 *  ✔ socket_cb/timer_cb настроены и работают через reactor
 *  ✔ curl_multi_socket_action() вызывается по IO/таймер событиям
 *  ✔ завершение запросов обрабатывается (CURLMSG_DONE → done_cb)
 *
 * По сути, это "двигатель", который крутит libcurl multi поверх
 * абстрактного reactor'а, не зная ничего о деталях S3-протокола.
 */


/* ============================================================
 *        Контекст для одного сокета (fd), отслеживаемого CURLM
 * ========================================================== */

typedef struct s3_curl_socket_ctx {
    s3_client_t            *client;
    curl_socket_t           fd;
    s3_reactor_io_handle_t  io_handle;
} s3_curl_socket_ctx_t;

/* ============================================================
 *      Обработка завершённых запросов (CURLMSG_DONE)
 * ========================================================== */

static void
s3_client_multi_process_messages(s3_client_t *client)
{
    if (client == NULL || client->multi == NULL)
        return;

    int msgs_in_queue = 0;
    CURLMsg *msg = NULL;

    while ((msg = curl_multi_info_read(client->multi, &msgs_in_queue)) != NULL) {
        if (msg->msg != CURLMSG_DONE)
            continue;

        CURL *easy = msg->easy_handle;
        s3_request_t *req = NULL;
        curl_easy_getinfo(easy, CURLINFO_PRIVATE, (char **)&req);

        s3_error_t err;
        s3_error_clear(&err);

        if (msg->data.result == CURLE_OK) {
            err.code = S3_OK;
            (void)snprintf(err.message, sizeof(err.message),
                           "OK");
        } else {
            err.code = S3_E_INTERNAL;
            (void)snprintf(err.message, sizeof(err.message),
                           "libcurl error: %d", (int)msg->data.result);
        }

        if (req != NULL && req->done_cb != NULL) {
            req->done_cb(req, &err, req->user_data);
        }

        curl_multi_remove_handle(client->multi, easy);
        curl_easy_cleanup(easy);

        if (req != NULL) {
            free(req);
        }
    }
}

/* ============================================================
 *        Вспомогательные функции для reactor <-> CURLM
 * ========================================================== */

static int
s3_curl_events_from_curl(int what)
{
    int events = S3_IO_EVENT_NONE;
    if (what == CURL_POLL_IN || what == CURL_POLL_INOUT)
        events |= S3_IO_EVENT_READ;
    if (what == CURL_POLL_OUT || what == CURL_POLL_INOUT)
        events |= S3_IO_EVENT_WRITE;
    return events;
}

static void
s3_curl_reactor_io_cb(int events, void *cb_data)
{
    s3_curl_socket_ctx_t *ctx = (s3_curl_socket_ctx_t *)cb_data;

    if (ctx == NULL) {
        fprintf(stderr,
                "s3_curl_reactor_io_cb: ctx == NULL (events=%d)\n",
                events);
        return;
    }

    if (ctx->client == NULL) {
        fprintf(stderr,
                "s3_curl_reactor_io_cb: ctx=%p client=NULL fd=%d events=%d\n",
                (void *)ctx, (int)ctx->fd, events);
        return;
    }

    if (ctx->client->multi == NULL) {
        fprintf(stderr,
                "s3_curl_reactor_io_cb: ctx=%p client=%p multi=NULL fd=%d events=%d\n",
                (void *)ctx, (void *)ctx->client, (int)ctx->fd, events);
        return;
    }

    s3_client_t *client = ctx->client;

    int action = 0;
    if (events & S3_IO_EVENT_READ)
        action |= CURL_CSELECT_IN;
    if (events & S3_IO_EVENT_WRITE)
        action |= CURL_CSELECT_OUT;

    int running = 0;
    CURLMcode mrc = curl_multi_socket_action(
        client->multi,
        ctx->fd,
        action,
        &running);

    client->still_running = running;

    if (mrc != CURLM_OK) {
        fprintf(stderr,
                "s3_curl_reactor_io_cb: curl_multi_socket_action failed: %d\n",
                (int)mrc);
        return;
    }

    /* Проверить завершённые запросы. */
    s3_client_multi_process_messages(client);
}


/* reactor → сработал таймер */
static void
s3_curl_reactor_timer_cb(void *cb_data)
{
    s3_client_t *client = (s3_client_t *)cb_data;
    if (client == NULL || client->multi == NULL)
        return;

    int running = 0;
    CURLMcode mrc = curl_multi_socket_action(client->multi,
                                             CURL_SOCKET_TIMEOUT,
                                             0,
                                             &running);
    client->still_running = running;

    if (mrc != CURLM_OK) {
        /* TODO: логирование/обработка ошибок CURLM. */
    }

    /* Проверить завершённые запросы. */
    s3_client_multi_process_messages(client);
}

/* ============================================================
 *          Статические callbacks для CURLM
 * ========================================================== */

/*
 * socket_cb — вызывается libcurl, когда меняется интерес к сокету.
 *
 * what:
 *   - CURL_POLL_IN    — слушать на чтение (READ)
 *   - CURL_POLL_OUT   — слушать на запись (WRITE)
 *   - CURL_POLL_INOUT — и чтение, и запись
 *   - CURL_POLL_REMOVE— больше не интересует этот сокет
 *
 * socketp:
 *   - указатель, который мы ассоциируем с данным fd через curl_multi_assign();
 *   - используем его для хранения s3_curl_socket_ctx_t.
 */
static int
s3_curl_multi_socket_cb(CURL *easy,
                        curl_socket_t s,
                        int what,
                        void *userp,
                        void *socketp)
{
    (void)easy;

    s3_client_t *client = (s3_client_t *)userp;
    if (client == NULL)
        return 0;

    s3_reactor_t        *reactor = &client->cfg.reactor;
    s3_curl_socket_ctx_t *ctx    = (s3_curl_socket_ctx_t *)socketp;

    if (what == CURL_POLL_REMOVE) {
        /* Больше не интересует этот сокет. Отписываемся и чистим контекст. */
        if (ctx != NULL) {
            if (reactor->vtable && reactor->vtable->io_unsubscribe && ctx->io_handle) {
                reactor->vtable->io_unsubscribe(reactor->reactor_ud, ctx->io_handle);
            }
            free(ctx);
            curl_multi_assign(client->multi, s, NULL);
        }
        return 0;
    }

    int events = s3_curl_events_from_curl(what);

    if (ctx == NULL) {
        /* Новый сокет: создаём контекст и подписываемся в reactor'е. */
        ctx = (s3_curl_socket_ctx_t *)malloc(sizeof(s3_curl_socket_ctx_t));
        if (ctx == NULL) {
            return 0;
        }
        ctx->client    = client;
        ctx->fd        = s;
        ctx->io_handle = NULL;

        if (reactor->vtable && reactor->vtable->io_subscribe) {
            ctx->io_handle = reactor->vtable->io_subscribe(
                reactor->reactor_ud,
                (int)s,
                events,
                s3_curl_reactor_io_cb,
                ctx
            );
        }

        if (ctx->io_handle == NULL) {
            free(ctx);
            curl_multi_assign(client->multi, s, NULL);
            return 0;
        }

        curl_multi_assign(client->multi, s, ctx);
    } else {
        /* Существующий сокет: обновляем маску интересующих событий. */
        if (reactor->vtable && reactor->vtable->io_update && ctx->io_handle) {
            reactor->vtable->io_update(
                reactor->reactor_ud,
                ctx->io_handle,
                events
            );
        }
    }

    return 0;
}

/*
 * timer_cb — вызывается libcurl, когда нужно установить (или отменить) таймер.
 *
 * timeout_ms:
 *   - >=0 — нужно установить таймер на timeout_ms миллисекунд;
 *   -  -1 — таймер можно отменить.
 */
static int
s3_curl_multi_timer_cb(CURLM *multi,
                       long timeout_ms,
                       void *userp)
{
    (void)multi;

    s3_client_t *client = (s3_client_t *)userp;
    if (client == NULL)
        return 0;

    s3_reactor_t *reactor = &client->cfg.reactor;

    if (timeout_ms < 0) {
        /* Отменяем таймер. */
        if (client->timer_handle && reactor->vtable && reactor->vtable->timer_cancel) {
            reactor->vtable->timer_cancel(reactor->reactor_ud, client->timer_handle);
            client->timer_handle = NULL;
        }
        return 0;
    }

    /* Переустанавливаем таймер. */
    if (client->timer_handle && reactor->vtable && reactor->vtable->timer_cancel) {
        reactor->vtable->timer_cancel(reactor->reactor_ud, client->timer_handle);
        client->timer_handle = NULL;
    }

    if (reactor->vtable && reactor->vtable->timer_start) {
        client->timer_handle = reactor->vtable->timer_start(
            reactor->reactor_ud,
            (unsigned int)timeout_ms,
            s3_curl_reactor_timer_cb,
            client
        );
    }

    return 0;
}

/* ============================================================
 *        Реализация internal-функций init/cleanup CURLM
 * ========================================================== */

int
s3_client_multi_init(s3_client_t *client)
{
    if (client == NULL)
        return -1;

    client->multi = curl_multi_init();
    if (client->multi == NULL) {
        return -1;
    }

    client->still_running = 0;
    client->timer_handle  = NULL;

    CURLMcode mrc;

    /* Настраиваем socket callback. */
    mrc = curl_multi_setopt(client->multi,
                            CURLMOPT_SOCKETFUNCTION,
                            s3_curl_multi_socket_cb);
    if (mrc != CURLM_OK) {
        curl_multi_cleanup(client->multi);
        client->multi = NULL;
        return -1;
    }

    mrc = curl_multi_setopt(client->multi,
                            CURLMOPT_SOCKETDATA,
                            (void *)client);
    if (mrc != CURLM_OK) {
        curl_multi_cleanup(client->multi);
        client->multi = NULL;
        return -1;
    }

    /* Настраиваем timer callback. */
    mrc = curl_multi_setopt(client->multi,
                            CURLMOPT_TIMERFUNCTION,
                            s3_curl_multi_timer_cb);
    if (mrc != CURLM_OK) {
        curl_multi_cleanup(client->multi);
        client->multi = NULL;
        return -1;
    }

    mrc = curl_multi_setopt(client->multi,
                            CURLMOPT_TIMERDATA,
                            (void *)client);
    if (mrc != CURLM_OK) {
        curl_multi_cleanup(client->multi);
        client->multi = NULL;
        return -1;
    }

    return 0;
}

void
s3_client_multi_cleanup(s3_client_t *client)
{
    if (client == NULL)
        return;

    s3_reactor_t *reactor = &client->cfg.reactor;

    if (client->timer_handle && reactor->vtable && reactor->vtable->timer_cancel) {
        reactor->vtable->timer_cancel(reactor->reactor_ud, client->timer_handle);
        client->timer_handle = NULL;
    }

    if (client->multi != NULL) {
        curl_multi_cleanup(client->multi);
        client->multi = NULL;
    }

    client->still_running = 0;
}
