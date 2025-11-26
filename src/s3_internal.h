#ifndef TARANTOOL_S3_INTERNAL_H_INCLUDED
#define TARANTOOL_S3_INTERNAL_H_INCLUDED 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

#include "s3/client.h"
#include "s3/alloc.h"

struct s3_http_backend_impl;

/*
 * Виртуальная таблица backend'а HTTP (curl_easy / curl_multi).
 *
 * Все методы:
 *  - возвращают s3_error_code_t;
 *  - при error != NULL заполняют s3_error_t;
 *  - не трогают coio/файберы — считаем, что они вызываются уже на worker-треде.
 */
struct s3_http_backend_vtbl {
    s3_error_code_t
    (*put_fd)(struct s3_http_backend_impl *backend,
              const s3_put_opts_t *opts,
              int fd, off_t offset, size_t size,
              s3_error_t *error);

    s3_error_code_t
    (*get_fd)(struct s3_http_backend_impl *backend,
              const s3_get_opts_t *opts,
              int fd, off_t offset, size_t max_size,
              size_t *bytes_written,
              s3_error_t *error);

    s3_error_code_t
    (*create_bucket)(struct s3_http_backend_impl *backend,
                   const s3_create_bucket_opts_t *opts,
                   s3_error_t *error);

    s3_error_code_t
    (*list_objects)(struct s3_http_backend_impl *backend,
                    const s3_list_objects_opts_t *opts,
                    s3_list_objects_result_t *out,
                    s3_error_t *error);

    s3_error_code_t
    (*delete_objects)(struct s3_http_backend_impl *backend,
                      const s3_delete_objects_opts_t *opts,
                      s3_error_t *error);

    void
    (*destroy)(struct s3_http_backend_impl *backend);
};

/*
 * Базовая структура backend'а.
 * Конкретные реализации (easy/multi) её "расширяют".
 */
struct s3_http_backend_impl {
    const struct s3_http_backend_vtbl *vtbl;
    struct s3_client *client;
};

/*
 * Реальная структура клиента.
 * Пользователь видит её как opaque s3_client_t.
 */
struct s3_client {
    s3_allocator_t alloc;

    char *endpoint;
    char *region;
    char *access_key;
    char *secret_key;
    char *session_token;
    char *default_bucket;

    /* Таймауты и флаги (уже с подставленными дефолтами). */
    uint32_t connect_timeout_ms;
    uint32_t request_timeout_ms;
    uint32_t max_total_connections;
    uint32_t max_connections_per_host;
    uint32_t multi_idle_timeout_ms;

    const char *ca_file;
    const char *ca_path;
    const char *proxy;

    uint32_t flags;

    bool require_sigv4;

    /* Тип и конкретная реализация HTTP backend'а. */
	s3_http_backend_t backend_type;
    struct s3_http_backend_impl *backend;

    /* Последняя ошибка (для s3_client_last_error). */
    s3_error_t last_error;
};

/*
 * Вспомогательный strdup поверх нашего аллокатора.
 * При ошибке:
 *  - возвращает NULL;
 *  - если err != NULL, заполняет err.code = S3_E_NOMEM и message.
 */
char *
s3_strdup_a(const s3_allocator_t *a, const char *src, s3_error_t *err);

/*
 * Инициализировать last_error клиента указанной ошибкой.
 * Если src == NULL, устанавливает S3_E_INTERNAL.
 */
void
s3_client_set_error(struct s3_client *client, const s3_error_t *src);

/*
 * Фабрики backend'ов (curl_easy / curl_multi).
 * При ошибке возвращают NULL и заполняют error (если не NULL).
 */
struct s3_http_backend_impl *
s3_http_easy_backend_new(struct s3_client *client, s3_error_t *error);

struct s3_http_backend_impl *
s3_http_multi_backend_new(struct s3_client *client, s3_error_t *error);

/*
 * Глобальная инициализация libcurl.
 * Вызывается один раз (pthread_once) при первом создании клиента.
 */
s3_error_code_t
s3_curl_global_init(s3_error_t *error);

/*
 * Вспомогательные функции для работы с s3_error_t,
 * доступны во всех внутренних модулях.
 */

/* Обнулить ошибку и выставить code = S3_E_OK. */
void
s3_error_clear(s3_error_t *err);

/* Заполнить структуру ошибки. msg можно NULL. */
void
s3_error_set(s3_error_t *err, s3_error_code_t code,
             const char *msg, int os_error,
             int http_status, long curl_code);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TARANTOOL_S3_INTERNAL_H_INCLUDED */
