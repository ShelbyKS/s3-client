
#ifndef S3_CONFIG_H_INCLUDED
#define S3_CONFIG_H_INCLUDED

/*
 * s3_config.h
 *
 * Конфигурационные структуры для инициализации:
 *   - глобальной среды (s3_env_config)
 *   - клиента (s3_client_config)
 *
 * Эти структуры определяют параметры, которые действуют
 * длительное время (lifecycle уровня процесса и клиента).
 * Они не меняются от запроса к запросу.
 */

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Пользовательский аллокатор */
struct s3_allocator {
    void *(*malloc_fn)(void *ud, size_t size);
    void  (*free_fn)(void *ud, void *ptr);
    void  *ud; /* произвольный контекст (arena, small_alloc и т.п.) */
};

/* ----------------------------------------------------------------------
 * 1. Конфигурация глобальной среды S3-клиента (s3_env)
 * ---------------------------------------------------------------------- */

/**
 * Конфигурация инициализации глобальной среды.
 *
 * Эта структура задаёт настройки уровня процесса:
 *   - реиспользование соединений
 *   - будущее место для настройки curl_multi, TLS и т.п.
 *
 * Объект s3_env_t создаётся один раз на процесс.
 */
struct s3_env_config {
    /**
     * Включить переиспользование HTTP-соединений (connection pooling).
     *
     * В реализации это может быть поддержано через:
     *   - curl_share с CURL_LOCK_DATA_CONNECT
     *   - собственные механизмы библиотеки
     *
     * Значения:
     *   0 — выкл. (по умолчанию)
     *   1 — включить pooling
     */
    int curl_share_connections;

    /*
     * Необязательный пользовательский аллокатор.
     *
     * Если NULL:
     *   - библиотека использует стандартный malloc/free.
     *
     * Если задан:
     *   - ВСЕ внутренние аллокации, завязанные на s3_env_t,
     *     будут использовать этот аллокатор (env, client и т.п.).
     */
    const struct s3_allocator *allocator;
};

/* ----------------------------------------------------------------------
 * 2. Конфигурация клиента (s3_client)
 * ---------------------------------------------------------------------- */

/**
 * Конфигурация клиента S3.
 *
 * Клиент определяет:
 *   - endpoint и регион
 *   - креды SigV4
 *   - TLS режим
 *   - default bucket
 *   - таймауты
 *
 * Клиент создаётся один раз и используется для всех операций.
 */
struct s3_client_config {
    /**
     * Endpoint S3 или MinIO.
     * Примеры:
     *   "https://s3.amazonaws.com"
     *   "https://s3.eu-central-1.amazonaws.com"
     *   "http://localhost:9000"
     *
     * Обязательное поле.
     */
    const char *endpoint;

    /**
     * Регион для SigV4 (AWS-style).
     * Примеры:
     *   "us-east-1"
     *   "eu-central-1"
     *
     * Для MinIO обычно может быть "us-east-1".
     */
    const char *region;

    /**
     * Аккаунт S3:
     *   - access key
     *   - secret key
     *
     * Обязательные поля.
     */
    const char *access_key;
    const char *secret_key;

    /**
     * Сессионный AWS-токен (опционально).
     * Может быть NULL.
     */
    const char *session_token;

    /**
     * Default bucket.
     * Может быть NULL — тогда bucket в операциях нужно указывать явно.
     */
    const char *default_bucket;

    /**
     * Использовать HTTPS.
     *   1 — да (по умолчанию)
     *   0 — HTTP
     */
    int use_https;

    /**
     * TLS: проверять сертификат (verify peer).
     *   1 — проверять
     *   0 — отключить (не рекомендуется)
     */
    int verify_peer;

    /**
     * TLS: проверять имя хоста (verify host).
     *   1 — проверять
     *   0 — отключить
     */
    int verify_host;

    /**
     * Максимальное время на установление соединения (мс).
     * 0 = дефолтные настройки.
     */
    long connect_timeout_ms;

    /**
     * Максимальное время на выполнение HTTP-запроса (мс).
     * 0 = дефолтное значение.
     */
    long request_timeout_ms;
};

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* S3_CONFIG_H_INCLUDED */
