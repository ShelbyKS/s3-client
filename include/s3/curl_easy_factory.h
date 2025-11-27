#ifndef TARANTOOL_S3_CURL_EASY_FACTORY_H_INCLUDED
#define TARANTOOL_S3_CURL_EASY_FACTORY_H_INCLUDED 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <sys/types.h>
#include <s3/curl_compat.h>

#include "s3/client.h"

typedef enum {
    S3_IO_NONE = 0,
    S3_IO_FD,
    S3_IO_MEM,
} s3_easy_io_kind_t;

typedef struct s3_mem_buf {
    char   *data;
    size_t  size;
    size_t  capacity;
} s3_mem_buf_t;

/*
 * Описание I/O для easy-хендла.
 *
 * read_io  — используется для исходящего тела (PUT/POST).
 * write_io — для входящего тела (GET).
 *
 */
typedef struct s3_easy_io {
    /*
     * kind:
     *   - S3_IO_FD  — работа с файловым дескриптором (pread/pwrite)
     *   - S3_IO_MEM — запись/чтение в память (s3_mem_buf_t)
     *   - S3_IO_NONE — не использовать
    */
    s3_easy_io_kind_t kind;

    /*
    * size_limit — максимум байт для чтения/записи:
    *   - для PUT: сколько максимум отправить;
    *   - для GET: сколько максимум принять (0 = без ограничения).
    */
    size_t size_limit;

    union {
        struct {
            int   fd;
            /* offset — начальная позиция (используется с pread/pwrite) */
            off_t offset;
        } fd;

        struct {
            s3_mem_buf_t *buf; /* не владеем, буфер живёт снаружи */
        } mem;
    } u;
} s3_easy_io_t;


/* Хэлперы для инициализации I/O */
static inline void
s3_easy_io_init_none(s3_easy_io_t *io)
{
    io->kind = S3_IO_NONE;
    io->size_limit = 0;
}

static inline void
s3_easy_io_init_fd(s3_easy_io_t *io, int fd, off_t offset, size_t size_limit)
{
    io->kind = S3_IO_FD;
    io->u.fd.fd = fd;
    io->u.fd.offset = offset;
    io->size_limit = size_limit;
}

static inline void
s3_easy_io_init_mem(s3_easy_io_t *io, s3_mem_buf_t *buf, size_t size_limit)
{
    io->kind = S3_IO_MEM;
    io->u.mem.buf = buf;
    io->size_limit = size_limit;
}

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
    s3_easy_io_t read_io;   /* для PUT/POST (исходящее тело), может быть "пустым" */
    s3_easy_io_t write_io;  /* для GET/LIST (входящее тело), может быть "пустым" */

    /* Внутреннее состояние, чтобы callbacks знали, сколько уже прочитали/написали. */
    size_t read_bytes_total;
    size_t write_bytes_total;

    /* Тело запроса, если мы его сами строим (DELETE, POST, и т.п.) */
    s3_mem_buf_t owned_body;
    /* Тело ответа, если хотим его собрать целиком (LIST, DELETE, и т.п.) */
    s3_mem_buf_t owned_resp;
};

/*
 * Создаёт полностью сконфигурированный CURL easy для PUT.
 *
 * client                 — s3_client, из него берём endpoint/region/keys/таймауты.
 * opts                   — опции PUT (bucket, key, content_type, и т.п.).
 * fd/offset/size_limit   — описание источника данных.
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
s3_easy_factory_new_put_fd(s3_client_t *client,
                        const s3_put_opts_t *opts,
                        int fd, off_t offset, size_t size,
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
s3_easy_factory_new_get_fd(s3_client_t *client,
                        const s3_get_opts_t *opts,
                        int fd, off_t offset, size_t max_size,
                        s3_easy_handle_t **out_handle,
                        s3_error_t *error);

s3_error_code_t
s3_easy_factory_new_create_bucket(s3_client_t *client,
                                  const s3_create_bucket_opts_t *opts,
                                  s3_easy_handle_t **out_handle,
                                  s3_error_t *error);

s3_error_code_t
s3_easy_factory_new_list_objects(s3_client_t *client,
                                 const s3_list_objects_opts_t *opts,
                                 s3_easy_handle_t **out_handle,
                                 s3_error_t *error);

s3_error_code_t
s3_easy_factory_new_delete_objects(s3_client_t *client,
                                   const s3_delete_objects_opts_t *opts,
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
