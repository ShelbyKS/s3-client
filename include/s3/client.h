#ifndef S3_CLIENT_H
#define S3_CLIENT_H

/*
 *  s3/client.h
 *
 *  Публичный API асинхронного S3-клиента, построенного поверх libcurl
 *  (multi interface) и внешнего reactor'а (см. s3/reactor.h).
 *
 *  Основные свойства:
 *    - клиент НЕ крутит свой event loop;
 *    - интеграция с любым циклом событий через s3_reactor_t;
 *    - object_get / object_put — асинхронные, с callback'ами;
 *    - есть удобные обёртки для работы с памятью (buffer) и файлами (fd);
 *    - низкоуровневые функции s3_object_get/s3_object_put доступны как
 *      advanced API.
 */

#include <stddef.h>
#include <stdint.h>

#include <s3/config.h>  /* s3_client_config_t, s3_log_fn и т.д. */
#include <s3/reactor.h> /* s3_reactor_t */
#include <s3/types.h>   /* параметры GET/PUT, буферы, callbacks */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 *                       Версия библиотеки
 * ========================================================== */

#define S3_CLIENT_VERSION_MAJOR 0
#define S3_CLIENT_VERSION_MINOR 1
#define S3_CLIENT_VERSION_PATCH 0

static inline uint32_t
s3_client_version_u32(void)
{
    return ((uint32_t)S3_CLIENT_VERSION_MAJOR << 16) |
           ((uint32_t)S3_CLIENT_VERSION_MINOR << 8)  |
           (uint32_t)S3_CLIENT_VERSION_PATCH;
}

/* ============================================================
 *                       Типы вперёд
 * ========================================================== */

/* Opaque-типы клиента и запроса. */
typedef struct s3_client  s3_client_t;
typedef struct s3_request s3_request_t;

/* ============================================================
 *                         Ошибки
 * ========================================================== */

typedef enum s3_error_code {
    S3_OK = 0,

    /* Конфигурация / параметры */
    S3_E_INVALID_ARGUMENT,
    S3_E_OUT_OF_MEMORY,

    /* Ошибки инициализации/работы libcurl */
    S3_E_CURL_GLOBAL,
    S3_E_CURL_INIT,
    S3_E_CURL_PERFORM,

    /* Таймауты / управление запросом */
    S3_E_TIMEOUT,
    S3_E_CANCELLED,

    /* HTTP / S3 протокол */
    S3_E_HTTP,
    S3_E_S3_ERROR,

    /* Внутренние ошибки реализации */
    S3_E_INTERNAL
} s3_error_code_t;

typedef struct s3_error {
    s3_error_code_t code;        /* код ошибки */
    long            http_status; /* HTTP статус, если применимо (0 — нет) */
    int             curl_code;   /* CURLcode (libcurl), если есть (0 — нет) */
    char            message[256];/* человекочитаемое описание, null-terminated */
} s3_error_t;

/* Строковое имя кода ошибки (для логов/отладчика). */
const char *s3_error_code_str(s3_error_code_t code);

/* Заполнить s3_error_t значением "OK". */
void s3_error_clear(s3_error_t *err);

/* ============================================================
 *              Создание / уничтожение клиента
 * ========================================================== */

/**
 * Создать новый S3-клиент.
 *
 *  - cfg: указатель на конфигурацию (endpoint, region, credentials,
 *         reactor, таймауты и т.д.).
 *
 * Возвращает:
 *  - указатель на s3_client_t при успехе;
 *  - NULL при ошибке (например, неверные аргументы или OOM).
 *
 * Важно:
 *  - один клиент обычно привязан к одному reactor/event loop;
 *  - thread-safety не гарантируется: использование из одного потока
 *    или под внешней синхронизацией.
 */
s3_client_t *
s3_client_new(const s3_client_config_t *cfg);

/**
 * Уничтожить клиент и освободить все связанные ресурсы.
 *
 * Требования:
 *  - к моменту вызова все запросы должны быть завершены или
 *    отменены через s3_request_cancel();
 *  - вызывать только из того же контекста (потока), где живёт клиент.
 */
void
s3_client_destroy(s3_client_t *client);

/* ============================================================
 *                 Управление отдельным запросом
 * ========================================================== */

/**
 * Отменить запрос.
 *
 *  - После отмены финальный s3_request_done_cb всё равно будет вызван
 *    (с error.code = S3_E_CANCELLED или другим кодом по усмотрению
 *     реализации).
 *  - Отмена может быть "ленивой": фактическое завершение произойдёт,
 *    когда libcurl/loop обработает событие.
 */
void
s3_request_cancel(s3_request_t *req);

/* ============================================================
 *          Низкоуровневый асинхронный API (advanced)
 *          object_get / object_put с произвольным стримингом
 * ========================================================== */

/**
 * Асинхронный GET объекта.
 *
 * Advanced API:
 *   - Позволяет реализовать произвольный стриминг данных через data_cb.
 *   - Для типичных случаев (буфер/файл) см. удобные обёртки ниже:
 *       s3_object_get_to_buffer()
 *       s3_object_get_to_fd()
 *
 *  - params:      описание объекта (bucket, key, range, etag-условия и т.д.).
 *  - headers_cb:  вызывается после получения HTTP-заголовков (может быть NULL).
 *  - data_cb:     вызывается для каждого куска данных (может быть NULL — тогда
 *                 тело ответа будет просто отброшено).
 *  - done_cb:     вызывается один раз при завершении запроса (может быть NULL).
 *  - user_data:   прокидывается во все callbacks.
 *
 * Возвращает:
 *  - указатель на s3_request_t при успешном запуске;
 *  - NULL, если запрос не смог стартовать (ошибка параметров/памяти).
 */
s3_request_t *
s3_object_get(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    s3_object_get_headers_cb         headers_cb,
    s3_object_get_data_cb            data_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data);

/**
 * Асинхронный PUT объекта.
 *
 * Advanced API:
 *   - Позволяет реализовать произвольный источник данных через read_cb
 *     (стриминг из файлов, сокетов, генерируемых потоков, pipe'ов и т.д.).
 *   - Для типичных случаев (буфер/файл) см. удобные обёртки ниже:
 *       s3_object_put_from_buffer()
 *       s3_object_put_from_fd()
 *
 *  - params:   описание объекта (bucket, key, content_length, content_type и т.д.).
 *  - read_cb:  вызывается библиотекой, когда libcurl нужны данные для отправки.
 *  - done_cb:  вызывается при завершении запроса (может быть NULL).
 *  - user_data: прокидывается во все callbacks.
 *
 * Возвращает:
 *  - указатель на s3_request_t при запуске;
 *  - NULL при ошибке до старта.
 */
s3_request_t *
s3_object_put(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    s3_object_put_read_cb            read_cb,
    s3_request_done_cb               done_cb,
    void                            *user_data);

/* ============================================================
 *        Удобные обёртки: буферы и файловые дескрипторы
 * ========================================================== */

/**
 * GET в заранее выделенный буфер.
 *
 *  - buf->data / buf->size должны быть инициализированы заранее;
 *  - buf->used будет увеличиваться по мере прихода данных;
 *  - если данных придёт больше, чем buf->size, запрос завершается с ошибкой.
 *
 * Реализовано поверх s3_object_get() с внутренним data_cb.
 */
s3_request_t *
s3_object_get_to_buffer(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    s3_buffer_t                     *buf,
    s3_object_get_headers_cb         headers_cb, /* может быть NULL */
    s3_request_done_cb               done_cb,    /* может быть NULL */
    void                            *user_data);

/**
 * GET в файловый дескриптор.
 *
 *  - fd должен быть открыт на запись;
 *  - offset — смещение, с которого начинать запись (обычно 0).
 *
 * Реализовано поверх s3_object_get() с внутренним data_cb.
 * Детали блокирующего/неблокирующего I/O (coio, thread-pool и т.п.)
 * зависят от уровня, который вызывает эту функцию (например, Tarantool-модуля).
 */
s3_request_t *
s3_object_get_to_fd(
    s3_client_t                     *client,
    const s3_object_get_params_t    *params,
    int                              fd,
    int64_t                          offset,
    s3_object_get_headers_cb         headers_cb, /* может быть NULL */
    s3_request_done_cb               done_cb,    /* может быть NULL */
    void                            *user_data);

/**
 * PUT из буфера.
 *
 *  - buf->data / buf->used содержат все отправляемые данные;
 *  - если params->content_length == (uint64_t)-1, реализация может
 *    использовать buf->used как фактический размер.
 *
 * Реализовано поверх s3_object_put() с внутренним read_cb.
 */
s3_request_t *
s3_object_put_from_buffer(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    const s3_buffer_t               *buf,
    s3_request_done_cb               done_cb,    /* может быть NULL */
    void                            *user_data);

/**
 * PUT из файлового дескриптора.
 *
 *  - fd должен быть открыт на чтение;
 *  - offset — смещение, с которого читать;
 *  - limit — сколько байт отправить; если (uint64_t)-1, читаем до EOF.
 *
 * Реализовано поверх s3_object_put() с внутренним read_cb.
 * Конкретный слой (например, Tarantool-модуль) может реализовать внутри
 * read_cb чтение через coio/thread-pool, чтобы не блокировать event loop.
 */
s3_request_t *
s3_object_put_from_fd(
    s3_client_t                     *client,
    const s3_object_put_params_t    *params,
    int                              fd,
    int64_t                          offset,
    uint64_t                         limit,      /* (uint64_t)-1 => до EOF */
    s3_request_done_cb               done_cb,    /* может быть NULL */
    void                            *user_data);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_CLIENT_H */
