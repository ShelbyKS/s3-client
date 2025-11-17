/* src/reactor/reactor_tarantool.c
 *
 * Адаптер s3_reactor_t поверх event loop Tarantool'а (libev).
 *
 * ВАЖНО:
 *  - Tarantool сам создаёт и крутит ev_loop;
 *  - мы только вешаем свои ev_io/ev_timer на уже существующий loop;
 *  - никакого ev_run() здесь нет.
 */

#include <tarantool_ev.h>  /* корректный ev.h из Tarantool */
#include <s3-adapters/reactor_tarantool.h>
#include <stdlib.h>

/* ---------- Внутренние структуры-обёртки для libev ---------- */

typedef struct s3_tt_io {
    ev_io               io;
    s3_reactor_io_cb    cb;
    void               *cb_data;
} s3_tt_io_t;

typedef struct s3_tt_timer {
    ev_timer             timer;
    s3_reactor_timer_cb  cb;
    void                *cb_data;
} s3_tt_timer_t;

/* ---------- ev-колбэки, которые вызывает libev ---------- */

static void
s3_tt_io_ev_cb(struct ev_loop *loop, ev_io *w, int revents)
{
    (void)loop;

    s3_tt_io_t *h = (s3_tt_io_t *)w;
    int events = S3_IO_EVENT_NONE;

    if (revents & EV_READ)
        events |= S3_IO_EVENT_READ;
    if (revents & EV_WRITE)
        events |= S3_IO_EVENT_WRITE;

    if (h->cb != NULL && events != S3_IO_EVENT_NONE)
        h->cb(events, h->cb_data);
}

static void
s3_tt_timer_ev_cb(struct ev_loop *loop, ev_timer *w, int revents)
{
    (void)loop;
    (void)revents;

    s3_tt_timer_t *h = (s3_tt_timer_t *)w;
    if (h->cb != NULL)
        h->cb(h->cb_data);
}

/* ---------- Реализация vtable для s3_reactor_t ---------- */

static s3_reactor_io_handle_t
s3_tt_io_subscribe(void *reactor_ud,
                   int fd,
                   int events,
                   s3_reactor_io_cb cb,
                   void *cb_data)
{
    struct ev_loop *loop = (struct ev_loop *)reactor_ud;
    if (loop == NULL)
        return NULL;

    s3_tt_io_t *h = (s3_tt_io_t *)malloc(sizeof(s3_tt_io_t));
    if (h == NULL)
        return NULL;

    h->cb      = cb;
    h->cb_data = cb_data;

    int ev_events = 0;
    if (events & S3_IO_EVENT_READ)
        ev_events |= EV_READ;
    if (events & S3_IO_EVENT_WRITE)
        ev_events |= EV_WRITE;

    ev_io_init(&h->io, s3_tt_io_ev_cb, fd, ev_events);
    ev_io_start(loop, &h->io);

    return (s3_reactor_io_handle_t)h;
}

static void
s3_tt_io_update(void *reactor_ud,
                s3_reactor_io_handle_t handle,
                int events)
{
    struct ev_loop *loop = (struct ev_loop *)reactor_ud;
    s3_tt_io_t *h = (s3_tt_io_t *)handle;
    if (loop == NULL || h == NULL)
        return;

    int ev_events = 0;
    if (events & S3_IO_EVENT_READ)
        ev_events |= EV_READ;
    if (events & S3_IO_EVENT_WRITE)
        ev_events |= EV_WRITE;

    ev_io_stop(loop, &h->io);
    ev_io_set(&h->io, h->io.fd, ev_events);
    ev_io_start(loop, &h->io);
}

static void
s3_tt_io_unsubscribe(void *reactor_ud,
                     s3_reactor_io_handle_t handle)
{
    struct ev_loop *loop = (struct ev_loop *)reactor_ud;
    s3_tt_io_t *h = (s3_tt_io_t *)handle;
    if (loop == NULL || h == NULL)
        return;

    ev_io_stop(loop, &h->io);
    free(h);
}

static s3_reactor_timer_handle_t
s3_tt_timer_start(void *reactor_ud,
                  unsigned int timeout_ms,
                  s3_reactor_timer_cb cb,
                  void *cb_data)
{
    struct ev_loop *loop = (struct ev_loop *)reactor_ud;
    if (loop == NULL)
        return NULL;

    s3_tt_timer_t *h = (s3_tt_timer_t *)malloc(sizeof(s3_tt_timer_t));
    if (h == NULL)
        return NULL;

    h->cb      = cb;
    h->cb_data = cb_data;

    double timeout_sec = (double)timeout_ms / 1000.0;
    ev_timer_init(&h->timer, s3_tt_timer_ev_cb, timeout_sec, 0.0);
    ev_timer_start(loop, &h->timer);

    return (s3_reactor_timer_handle_t)h;
}

static void
s3_tt_timer_cancel(void *reactor_ud,
                   s3_reactor_timer_handle_t handle)
{
    struct ev_loop *loop = (struct ev_loop *)reactor_ud;
    s3_tt_timer_t *h = (s3_tt_timer_t *)handle;
    if (loop == NULL || h == NULL)
        return;

    ev_timer_stop(loop, &h->timer);
    free(h);
}

/* ---------- Готовый vtable и init-функция ---------- */

static const s3_reactor_vtable_t s3_tt_vtable = {
    .io_subscribe   = s3_tt_io_subscribe,
    .io_update      = s3_tt_io_update,
    .io_unsubscribe = s3_tt_io_unsubscribe,
    .timer_start    = s3_tt_timer_start,
    .timer_cancel   = s3_tt_timer_cancel
};

/*
 * Инициализация s3_reactor_t поверх libev-цикла Tarantool'а.
 *
 *  - loop — это struct ev_loop*, который крутит Tarantool;
 *           обычно его можно получить как ev_default_loop(0)
 *           в контексте Tarantool-процесса.
 */
int
s3_reactor_init_tarantool(s3_reactor_t *r, struct ev_loop *loop)
{
    if (r == NULL || loop == NULL)
        return -1;

    r->vtable    = &s3_tt_vtable;
    r->reactor_ud = (void *)loop;
    return 0;
}

int
s3_client_config_init_tarantool(s3_client_config_t *cfg,
                                struct ev_loop     *loop)
{
    if (cfg == NULL || loop == NULL)
        return -1;

    s3_client_config_init(cfg); /* generic init: обнуляет, ставит дефолты */

    s3_reactor_t r;
    if (s3_reactor_init_tarantool(&r, loop) != 0)
        return -1;

    cfg->reactor = r;
    return 0;
}
