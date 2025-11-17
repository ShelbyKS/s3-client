#ifndef S3_REACTOR_H
#define S3_REACTOR_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  s3/reactor.h
 *
 *  Абстракция REACTOR-паттерна для S3-клиента.
 *
 *  Клиент НЕ имеет собственного event loop.
 *  Он использует внешний loop (libev, libuv, epoll, Tarantool, kqueue, …)
 *  через единый интерфейс — s3_reactor_t.
 *
 *  Реактор:
 *    - подписывает fd на события READ/WRITE;
 *    - уведомляет клиента о готовности fd;
 *    - создаёт и управляет таймерами;
 *    - НЕ занимается I/O сам — только делегирует события наружу.
 *
 *  Это позволяет библиотеке быть полностью event-loop-agnostic.
 */

/* ============================================================
 *                         События I/O
 * ========================================================== */

typedef enum s3_io_events {
    S3_IO_EVENT_NONE  = 0,
    S3_IO_EVENT_READ  = 1 << 0,
    S3_IO_EVENT_WRITE = 1 << 1
} s3_io_events_t;

/* ============================================================
 *           Типы opaque-дескрипторов, создаваемых reactor-ом
 * ========================================================== */

typedef void *s3_reactor_io_handle_t;
typedef void *s3_reactor_timer_handle_t;

/* ============================================================
 *           Callback-и, которые reactor вызывает при событиях
 * ========================================================== */

/*
 * Callback готовности fd.
 *
 * events — битовая маска s3_IO_EVENT_*.
 * cb_data — данные, переданные при регистрации.
 */
typedef void (*s3_reactor_io_cb)(
    int   events,
    void *cb_data);

/*
 * Callback таймера.
 */
typedef void (*s3_reactor_timer_cb)(
    void *cb_data);

/* ============================================================
 *                     Reactor интерфейс
 * ========================================================== */

typedef struct s3_reactor_vtable {

    /*
     * Подписаться на события fd.
     *
     * reactor_ud — контекст реактора (указатель на event loop).
     *
     * Возвращает:
     *   - handle для последующих update/unsubscribe,
     *   - или NULL при ошибке.
     */
    s3_reactor_io_handle_t (*io_subscribe)(
        void                   *reactor_ud,
        int                     fd,
        int                     events,
        s3_reactor_io_cb        cb,
        void                   *cb_data);

    /*
     * Изменить маску подписанных событий.
     */
    void (*io_update)(
        void                   *reactor_ud,
        s3_reactor_io_handle_t  handle,
        int                     events);

    /*
     * Отписаться от событий и освободить handle.
     */
    void (*io_unsubscribe)(
        void                   *reactor_ud,
        s3_reactor_io_handle_t  handle);

    /*
     * Создать однократный таймер.
     *
     * timeout_ms — срабатывание через N мс.
     * cb — вызывается reactor-ом по истечении таймера.
     */
    s3_reactor_timer_handle_t (*timer_start)(
        void                    *reactor_ud,
        unsigned int             timeout_ms,
        s3_reactor_timer_cb      cb,
        void                    *cb_data);

    /*
     * Отменить ранее созданный таймер.
     */
    void (*timer_cancel)(
        void                      *reactor_ud,
        s3_reactor_timer_handle_t  handle);

} s3_reactor_vtable_t;


/* ============================================================
 *                     Обёртка над vtable
 * ========================================================== */

typedef struct s3_reactor {
    const s3_reactor_vtable_t *vtable;
    void                      *reactor_ud; /* event loop context */
} s3_reactor_t;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_REACTOR_H */
