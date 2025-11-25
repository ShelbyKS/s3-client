#ifndef TARANTOOL_S3_CURL_EASY_FACTORY_H_INCLUDED
#define TARANTOOL_S3_CURL_EASY_FACTORY_H_INCLUDED 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
// #include <curl/curl.h>
// #include <tarantool/curl.h>
#include <s3/curl_compat.h>

#include "s3/client.h"

/*
 * Описание I/O для easy-хендла.
 *
 * read_io  — используется для исходящего тела (PUT/POST).
 * write_io — для входящего тела (GET).
 *
 * fd         — файловый дескриптор.
 * offset     — начальная позиция (используется с pread/pwrite).
 * size_limit — максимум байт для чтения/записи:
 *              - для PUT: сколько максимум отправить;
 *              - для GET: сколько максимум принять (0 = без ограничения).
 */
typedef struct s3_easy_io {
    int   fd;
    off_t offset;
    size_t size_limit;
} s3_easy_io_t;

/*
 * Внутренняя обёртка над CURL *easy.
 * Пользователь её не видит, с ней работают только backend’ы.
 */
typedef struct s3_easy_handle s3_easy_handle_t;

struct s3_easy_handle {
    CURL *easy;
    struct curl_slist *headers;

    s3_client_t *client;
	char *url;

    /* Контекст для read/write callbacks. */
    s3_easy_io_t read_io;   /* для PUT (исходящее тело), может быть "пустым" */
    s3_easy_io_t write_io;  /* для GET (входящее тело), может быть "пустым" */

    /* Внутреннее состояние, чтобы callbacks знали, сколько уже прочитали/написали. */
    size_t read_bytes_total;
    size_t write_bytes_total;
};

/*
 * Создаёт полностью сконфигурированный CURL easy для PUT.
 *
 * client  — s3_client, из него берём endpoint/region/keys/таймауты.
 * opts    — опции PUT (bucket, key, content_type, и т.п.).
 * io      — описание источника данных (fd/offset/size_limit).
 *
 * При успехе:
 *   - возвращает S3_E_OK;
 *   - *out_handle указывает на новый s3_easy_handle_t.
 *
 * При ошибке:
 *   - возвращает код ошибки;
 *   - если error != NULL, заполняет структуру;
 *   - *out_handle не трогается.
 */
s3_error_code_t
s3_easy_factory_new_put(s3_client_t *client,
                        const s3_put_opts_t *opts,
                        const s3_easy_io_t *io,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error);

/*
 * Аналогично для GET.
 *
 * io.size_limit:
 *   - если 0 — принимаем весь объект;
 *   - если >0 — не пишем больше заданного лимита.
 */
s3_error_code_t
s3_easy_factory_new_get(s3_client_t *client,
                        const s3_get_opts_t *opts,
                        const s3_easy_io_t *io,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error);

s3_error_code_t
s3_easy_factory_new_create_bucket(s3_client_t *client,
                                  const s3_create_bucket_opts_t *opts,
                                  s3_easy_handle_t **out_handle,
                                  s3_error_t *error);

/*
 * Освобождает s3_easy_handle:
 *   - curl_slist_free_all(headers);
 *   - curl_easy_cleanup(easy);
 *   - освобождает саму структуру через аллокатор клиента.
 *
 * Безопасно звать с NULL.
 */
void
s3_easy_handle_destroy(s3_easy_handle_t *h);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TARANTOOL_S3_CURL_EASY_FACTORY_H_INCLUDED */
