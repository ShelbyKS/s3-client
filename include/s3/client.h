#ifndef TARANTOOL_S3_CLIENT_H_INCLUDED
#define TARANTOOL_S3_CLIENT_H_INCLUDED 1

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <stdbool.h>

#include "s3/alloc.h"

/*
 * Тип HTTP backend'а.
 * Реализуется через libcurl (easy/multi).
 */
typedef enum s3_http_backend {
    S3_HTTP_BACKEND_CURL_EASY  = 0,
    S3_HTTP_BACKEND_CURL_MULTI = 1,
} s3_http_backend_t;

/*
 * Коды ошибок верхнего уровня.
 * 0 — успех.
 */
typedef enum s3_error_code {
    S3_E_OK = 0,          /* Успех */

    S3_E_INVALID_ARG,     /* Некорректные аргументы API */
    S3_E_NOMEM,           /* Не хватило памяти */
    S3_E_INIT,            /* Ошибка инициализации (curl, потоки, и т.п.) */

    S3_E_CURL,            /* Ошибка libcurl */
    S3_E_HTTP,            /* HTTP статус != 2xx */
    S3_E_SIGV4,           /* Ошибка при формировании подписи AWS SigV4 */

    S3_E_IO,              /* Ошибка чтения/записи fd (pread/pwrite) */
    S3_E_TIMEOUT,         /* Таймаут */
    S3_E_NOT_FOUND,       /* Объект или bucket не найден */
    S3_E_AUTH,            /* Ошибка аутентификации/авторизации */
    S3_E_ACCESS_DENIED,   /* Нет прав на объект/операцию */

    S3_E_CANCELLED,       /* Операция отменена пользователем */
    S3_E_INTERNAL,        /* Внутренняя ошибка клиента */
} s3_error_code_t;


typedef struct s3_error {
    s3_error_code_t code; /* Всегда совпадает с возвращаемым значением функции */

    int http_status;   /* HTTP статус или 0, если не применимо */
    long curl_code;    /* CURLE_* или 0 (не завязываемся на curl/curl.h в API) */
    int os_error;      /* errno или 0 */

    /* Короткое человекочитаемое сообщение (обрезается по длине). */
    char message[128];
} s3_error_t;

#define S3_ERROR_INIT { S3_E_OK, 0, 0, 0, {0} }

/*
 * Возвращает строку для кода ошибки.
 * Не использует s3_error_t, только enum.
 */
const char *
s3_error_code_str(s3_error_code_t code);

/*
 * Возвращает человекочитаемое сообщение об ошибке.
 * Если err == NULL, возвращает строку для S3_E_INTERNAL.
 */
const char *
s3_error_message(const s3_error_t *err);


/*
 * Флаги настроек клиента.
 */
enum {
    /*
     * Не ставить AWS SigV4 подпись (для локального MinIO без auth и т.п.).
     */
    S3_CLIENT_F_DISABLE_SIGV4          = 1u << 0,

    /*
     * Не проверять сертификат peer (CURLOPT_SSL_VERIFYPEER=0).
     */
    S3_CLIENT_F_SKIP_PEER_VERIFICATION = 1u << 1,

    /*
     * Не проверять host/hostname (CURLOPT_SSL_VERIFYHOST=0).
     */
    S3_CLIENT_F_SKIP_HOSTNAME_VERIF    = 1u << 2,

    /*
     * Использовать path-style адреса (https://host/bucket/key)
     * вместо virtual-hosted-style (https://bucket.host/key).
     */
    S3_CLIENT_F_FORCE_PATH_STYLE       = 1u << 3,
};

/*
 * Настройки клиента.
 */
typedef struct s3_client_opts {
    const char *endpoint;     /* Например: "https://s3.example.com" */
    const char *region;       /* Например: "us-east-1" */

    const char *access_key;   /* AWS Access Key ID */
    const char *secret_key;   /* AWS Secret Access Key */
    const char *session_token;/* Опционально: для временных кредов */

    const char *default_bucket; /* Опционально: bucket по умолчанию */
    bool require_sigv4;         /* Опционально: false по умолчанию */

    s3_http_backend_t backend;      /* EASY или MULTI */

    /*
     * Опциональный кастомный аллокатор.
     * Если NULL — используются системные malloc/free/realloc.
     */
    const struct s3_allocator *allocator;

    uint32_t connect_timeout_ms;         /* 5s -> значение по умолчанию */
    uint32_t request_timeout_ms;         /* 30s -> значение по умолчанию */
    uint32_t max_total_connections;      /* 64 -> значение по умолчанию */
    uint32_t max_connections_per_host;   /* 16 -> значение по умолчанию */
    uint32_t multi_idle_timeout_ms;      /* 50ms -> значение по умолчанию */

    const char *ca_file;
    const char *ca_path;
    const char *proxy;

    uint32_t flags;
} s3_client_opts_t;


#define S3_CLIENT_OPTS_INIT {               \
    .endpoint = NULL,                       \
    .region = NULL,                         \
    .access_key = NULL,                     \
    .secret_key = NULL,                     \
    .default_bucket = NULL,                 \
    .require_sigv4 = false,                 \
    .backend = S3_HTTP_BACKEND_EASY,        \
    .allocator = NULL,                      \
    .connect_timeout_ms = 0,                \
    .request_timeout_ms = 0,                \
    .max_total_connections = 0,             \
    .max_connections_per_host = 0,          \
    .ca_file = NULL,                        \
    .ca_path = NULL,                        \
    .proxy = NULL,                          \
    .flags = 0,                             \
}

typedef struct s3_client s3_client_t;

/*
 * Создаёт новый клиент.
 * Возвращает S3_E_OK при успехе, код ошибки иначе.
 * При ошибке *out_client не изменяется.
 */
s3_error_code_t
s3_client_new(const s3_client_opts_t *opts,
              s3_client_t **out_client,
              s3_error_t *error);

/*
 * Уничтожает клиент и освобождает ресурсы.
 * Безопасно вызывать с NULL.
 */
void
s3_client_delete(s3_client_t *client);

/*
 * Опции для PUT.
 * Все строки должны жить на время вызова (копируются/используются только внутри).
 */
typedef struct s3_put_opts {
    const char *bucket; /* Если NULL — используется default_bucket из клиента */
    const char *key;    /* Object key (обязателен) */

    const char *content_type;  /* Опционально: "application/octet-stream" и т.п. */
    uint64_t content_length;   /* Если 0 — берём из size аргумента put_fd */

    uint32_t flags;    /* На будущее: например, disable_expect_100_continue и т.п. */
} s3_put_opts_t;

/*
 * PUT: отправка тела из fd.
 *
 * Вызывается из файбера на tx-треде.
 * Внутри использует coio_call, то есть блокирует только текущий файбер.
 *
 * fd     — открытый дескриптор файла.
 * offset — начальная позиция (используется pread).
 * size   — сколько байт отправить.
 *
 * Возвращает S3_E_OK при успехе.
 * При ошибке возвращает код ошибки и, если error != NULL, заполняет структуру.
 */
s3_error_code_t
s3_client_put_fd(s3_client_t *client,
                 const s3_put_opts_t *opts,
                 int fd, off_t offset, size_t size,
                 s3_error_t *error);


/*
 * Опции для GET.
 */
typedef struct s3_get_opts {
    const char *bucket;
    const char *key;          /* Object key (обязателен) */

    /*
     * HTTP Range, например: "bytes=0-1023".
     * Если NULL — скачиваем весь объект.
     */
    const char *range;

    uint32_t flags;
} s3_get_opts_t;

/*
 * GET: приём тела в fd.
 *
 * fd        — открытый дескриптор.
 * offset    — стартовая позиция (используется pwrite).
 * max_size  — максимум байт для записи. Если 0 — без ограничения, пока не конец ответа.
 *
 * При успехе:
 *   - code == S3_E_OK
 *   - если bytes_written != NULL, туда сохраняется количество записанных байт.
 */
s3_error_code_t
s3_client_get_fd(s3_client_t *client,
                 const s3_get_opts_t *opts,
                 int fd, off_t offset, size_t max_size,
                 size_t *bytes_written,
                 s3_error_t *error);


/*
 * Опции для CREATE bucket.
 */
typedef struct s3_create_bucket_opts {
    const char *bucket;  /* обязательный */
    const char *acl;     /* optional, TODO: "private", "public-read" */
    uint32_t flags;      /* TODO: region_via_body, object_lock и т.п. */
} s3_create_bucket_opts_t;

/*
 * Создание бакета.
 *
 * bucket — имя бакета обязательно.
 */
s3_error_code_t
s3_client_create_bucket(s3_client_t *client,
                      const s3_create_bucket_opts_t *opts,
                      s3_error_t *error);


typedef struct s3_object_info {
    char    *key;           /* имя объекта */
    uint64_t size;          /* Content-Length */
    char    *etag;          /* без кавычек */
    char    *last_modified; /* ISO8601 строка, как в S3 */
    char    *storage_class; /* например, "STANDARD" */

    /* на будущее: owner, metadata и т.п. */
} s3_object_info_t;

typedef struct s3_list_objects_result {
    s3_object_info_t *objects;
    size_t            count;

    /* продолжение (пагинация ListObjectsV2) */
    bool   is_truncated;
    char  *next_continuation_token;
} s3_list_objects_result_t;

typedef struct s3_list_objects_opts {
    const char *bucket;      /* если NULL — default_bucket */
    const char *prefix;      /* фильтр по префиксу, может быть NULL */
    uint32_t    max_keys;    /* 0 — использовать дефолт сервера */

    /* пагинация */
    const char *continuation_token; /* NULL для первой страницы */

    uint32_t flags;          /* на будущее (delimiter, fetch-owner и т.д.) */
} s3_list_objects_opts_t;

/*
 * Выполнить ListObjectsV2 и распарсить результат в s3_list_result_t.
 * Память под строки и массив objects — через allocator клиента.
 */
s3_error_code_t
s3_client_list_objects(s3_client_t *client,
                       const s3_list_objects_opts_t *opts,
                       s3_list_objects_result_t *out,
                       s3_error_t *error);

/*
 * Освободить всё, что было выделено внутри s3_list_result_t.
 */
void
s3_list_objects_result_destroy(s3_client_t *client, s3_list_objects_result_t *res);


typedef struct s3_delete_object {
    const char *key;   /* обязательный объектный ключ */
    const char *version_id; /* необязательный */
} s3_delete_object_t;

typedef struct s3_delete_objects_opts {
    const char           *bucket;   /* если NULL — берём default_bucket */
    const s3_delete_object_t *objects; /* массив ключей */
    size_t                count;    /* сколько элементов в objects */

    bool                  quiet;    /* <Quiet>true</Quiet> в XML */
    uint32_t              flags;    /* на будущее */
} s3_delete_objects_opts_t;

/*
 * Batch DeleteObjects (POST /bucket?delete) для не-версированного бакета.
 * Удаляет до N объектов за один HTTP-запрос.
 */
s3_error_code_t
s3_client_delete_objects(s3_client_t *client,
                         const s3_delete_objects_opts_t *opts,
                         s3_error_t *error);


/*
 * Возвращает последний error клиента (thread/fiber-local внутри клиента).
 *
 * Если err != NULL, структура заполняется.
 * Если у клиента нет сохранённой ошибки, err->code будет S3_E_OK.
 */
void
s3_client_last_error(const s3_client_t *client, s3_error_t *err);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* TARANTOOL_S3_CLIENT_H_INCLUDED */
