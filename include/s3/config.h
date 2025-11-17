#ifndef S3_CONFIG_H
#define S3_CONFIG_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 *  s3/config.h
 *
 *  Конфигурация S3-клиента.
 *
 *  Здесь описывается:
 *    - endpoint / region;
 *    - креды (access/secret/session);
 *    - таймауты и лимиты;
 *    - reactor (event loop абстракция);
 *    - логирование.
 */

#include <stdint.h>
#include <stddef.h>

#include <s3/reactor.h> /* s3_reactor_t */

/* ============================================================
 *                         Логирование
 * ========================================================== */

/*
 * Уровень логирования, который библиотека может генерировать.
 * Реализация клиента сама решает, что логировать на каком уровне.
 */
typedef enum s3_log_level {
    S3_LOG_DEBUG = 0,
    S3_LOG_INFO,
    S3_LOG_WARN,
    S3_LOG_ERROR
} s3_log_level_t;

/*
 * Callback для логирования.
 *
 *  - level     — уровень сообщения;
 *  - msg       — null-terminated строка (живет только в момент вызова);
 *  - user_data — произвольный указатель, заданный пользователем.
 *
 * Если log_fn == NULL, библиотека ничего не логирует.
 */
typedef void (*s3_log_fn)(
    s3_log_level_t level,
    const char    *msg,
    void          *user_data);

/* ============================================================
 *                       Креды
 * ========================================================== */

typedef struct s3_credentials {
    /*
     * Обязательные поля для "классического" S3:
     *   - access_key_id
     *   - secret_access_key
     *
     * Для анонимного доступа можно установить оба в NULL.
     */

    const char *access_key_id;      /* может быть NULL для анонимного доступа */
    const char *secret_access_key;  /* может быть NULL для анонимного доступа */

    /*
     * Необязательное поле: session token (STS / временные креды).
     * Если NULL, не используется.
     */
    const char *session_token;
} s3_credentials_t;

/* ============================================================
 *                      Конфиг клиента
 * ========================================================== */

/*
 * Основная конфигурация клиента.
 *
 * Минимально необходимое:
 *   - endpoint;
 *   - region;
 *   - credentials (или NULL-поля для анонимного доступа);
 *   - reactor (реализация event loop).
 *
 * Остальное имеет дефолты через s3_client_config_init_default().
 */
typedef struct s3_client_config {

    /*
     * S3 endpoint (без схемы).
     *
     * Примеры:
     *   - "s3.amazonaws.com"
     *   - "minio.local"
     *   - "storage.yandexcloud.net"
     */
    const char *endpoint;

    /*
     * Регион (для AWS Signature V4).
     *
     * Примеры:
     *   - "us-east-1"
     *   - "eu-central-1"
     *
     * Для S3-совместимых хранилищ может использоваться фиктивное значение,
     * но для AWS — обязательно корректное.
     */
    const char *region;

    /*
     * Использовать HTTPS (1) или HTTP (0).
     * По умолчанию (через init_default) — 1.
     */
    int endpoint_is_https;

    /*
     * Креды для S3.
     *
     * Если access_key_id/secret_access_key == NULL:
     *   - клиент может работать в анонимном режиме, если бэкенд разрешает.
     */
    s3_credentials_t credentials;

    /*
     * Таймауты (в миллисекундах).
     *
     *  - connect_timeout_ms:
     *      максимальное время установления TCP-соединения;
     *      0 => использовать внутренний дефолт (например, 5000 мс).
     *
     *  - request_timeout_ms:
     *      общий таймаут одного запроса (GET/PUT), включая все попытки;
     *      0 => без явного общего таймаута (только libcurl internal).
     *
     *  - idle_timeout_ms:
     *      время простоя соединения в пуле перед его закрытием;
     *      0 => использовать дефолт или отключить idle-таймаут.
     */
    uint32_t connect_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t idle_timeout_ms;

    /*
     * Лимиты на количество соединений.
     *
     *  - max_connections:
     *      максимальное количество одновременных HTTP-соединений
     *      к endpoint'у. 0 => использовать внутренний дефолт
     *      (например, 16).
     */
    uint32_t max_connections;

    /*
     * Настройки проверки TLS (актуально, если endpoint_is_https == 1).
     *
     *  - verify_peer:
     *      1 — проверять сертификат сервера (рекомендуется),
     *      0 — не проверять (НЕ безопасно, только для тестов/dev).
     *
     *  - verify_host:
     *      1 — проверять соответствие хоста в сертификате,
     *      0 — отключить проверку.
     *
     *  - ca_path / ca_file:
     *      путь к директории/файлу с CA сертификатами.
     *      Если NULL — libcurl использует системные настройки.
     */
    int         verify_peer;
    int         verify_host;
    const char *ca_path;
    const char *ca_file;

    /*
     * Reactor (event loop интеграция).
     *
     * Должен быть установлен пользователем:
     *   - reactor.vtable        — таблица функций реактора;
     *   - reactor.reactor_ud    — указатель на контекст loop'а.
     */
    s3_reactor_t reactor;

    /*
     * Логирование.
     *
     *  - log_fn:
     *      callback, который будет получать сообщения от библиотеки;
     *      NULL => логирование отключено.
     *
     *  - log_user_data:
     *      непрозрачный указатель, прокидываемый в log_fn.
     */
    s3_log_fn  log_fn;
    void      *log_user_data;

    /*
     * Дополнительные флаги/зарезервированные поля для будущих расширений.
     * Можно обнулить через s3_client_config_init_default().
     */
    uint32_t   _reserved_flags;
    void      *_reserved_ptr;

} s3_client_config_t;

/* ============================================================
 *                   Инициализация дефолтов
 * ========================================================== */

/*
 * Заполнить конфиг дефолтными значениями.
 *
 *  - устанавливает нули во все поля;
 *  - подставляет разумные дефолты для таймаутов, max_connections,
 *    verify_peer/verify_host (обычно 1), и т.п.;
 *  - НЕ заполняет endpoint/region/credentials/reactor — их
 *    обязан задать пользователь.
 *
 * Пример:
 *
 *    s3_client_config_t cfg;
 *    s3_client_config_init_default(&cfg);
 *    cfg.endpoint = "s3.amazonaws.com";
 *    cfg.region = "eu-central-1";
 *    cfg.credentials.access_key_id = "...";
 *    cfg.credentials.secret_access_key = "...";
 *    cfg.reactor.vtable = &my_reactor_vtable;
 *    cfg.reactor.reactor_ud = my_loop;
 *    s3_client_t *client = s3_client_new(&cfg);
 */
void
s3_client_config_init_default(s3_client_config_t *cfg);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_CONFIG_H */
